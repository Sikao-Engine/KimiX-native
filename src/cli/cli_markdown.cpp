// cli/cli_markdown.cpp - ANSI terminal markdown renderer (see cli_markdown.h).
//
// Reference: kimix/cli_impl/utils.py (render_markdown + the theme table +
// the inline/block helpers, revision 86b7bf6).  The Python regex passes are
// expressed as scanners over the same patterns; every literal (bars, markers,
// the "  " heading padding) is byte-identical.

#include "cli/cli_markdown.h"

#include <cstdint>

#include "cli/cli_common.h"
#include "cli/cli_print.h"

namespace kimix::cli {

namespace {

// ---------------------------------------------------------------------------
// Theme (utils.py:63-206 DEFAULT_MD_THEME, same keys/colours)
// ---------------------------------------------------------------------------

struct climd_style {
    int fg = -1;            // basic SGR foreground (-1 == none)
    int bg256 = -1;         // 256-colour background (-1 == none)
    kimix::string styles;   // raw style codes ("1", "1,4", "2", "9", "3")
    const char *bar_char = "";
    bool top_bar = false;
    bool bottom_bar = false;
    const char *prefix = "";
    const char *suffix = "";
    const char *char_field = ""; // hr's "char"
};

// Color values (printing.py Color enum): BRIGHT_YELLOW 93, YELLOW 33,
// BRIGHT_WHITE 97, BRIGHT_CYAN 96, GRAY_LIGHT 250, BRIGHT_BLACK 90,
// BRIGHT_BLUE 94, BLUE 34, BRIGHT_MAGENTA 95, MAGENTA 35, GREEN 32,
// CYAN 36, BRIGHT_GREEN 92, BRIGHT_RED 91.
constexpr int kBrightYellow = 93;
constexpr int kBrightGreen = 92;
constexpr int kBrightRed = 91;
constexpr int kYellow = 33;
constexpr int kBrightWhite = 97;
constexpr int kBrightCyan = 96;
constexpr int kGrayLight = 250;
constexpr int kBrightBlack = 90;
constexpr int kBrightBlue = 94;
constexpr int kBlue = 34;
constexpr int kBrightMagenta = 95;
constexpr int kMagenta = 35;
constexpr int kGreen = 32;
constexpr int kCyan = 36;

climd_style climd_theme_h1() {
    climd_style s;
    s.fg = kBrightYellow;
    s.bg256 = 17;
    s.styles = "1,4";
    s.bar_char = "═";
    s.top_bar = true;
    s.bottom_bar = true;
    s.prefix = " ";
    s.suffix = " ";
    return s;
}

climd_style climd_theme_h2() {
    climd_style s;
    s.fg = kBrightYellow;
    s.bg256 = 17;
    s.styles = "1";
    s.bar_char = "─";
    s.bottom_bar = true;
    s.prefix = " ";
    s.suffix = " ";
    return s;
}

climd_style climd_theme_h3() {
    climd_style s;
    s.fg = kYellow;
    s.styles = "1,4";
    return s;
}

climd_style climd_theme_h4() {
    climd_style s;
    s.fg = kYellow;
    s.styles = "1";
    return s;
}

climd_style climd_theme_h5() {
    climd_style s;
    s.fg = kBrightWhite;
    s.styles = "1,2";
    return s;
}

climd_style climd_theme_h6() {
    climd_style s;
    s.fg = kGrayLight;
    s.styles = "1";
    return s;
}

climd_style climd_theme(int level) {
    switch (level) {
    case 1:
        return climd_theme_h1();
    case 2:
        return climd_theme_h2();
    case 3:
        return climd_theme_h3();
    case 4:
        return climd_theme_h4();
    case 5:
        return climd_theme_h5();
    default:
        return climd_theme_h6();
    }
}

climd_style climd_code_fence() {
    climd_style s;
    s.fg = kGrayLight;
    s.styles = "1,2";
    return s;
}

climd_style climd_code_block() {
    climd_style s;
    s.fg = kGreen;
    s.bg256 = 232;
    return s;
}

climd_style climd_strikethrough() {
    climd_style s;
    s.fg = kBrightBlack;
    s.styles = "9,2";
    return s;
}

climd_style climd_link_text() {
    climd_style s;
    s.fg = kBrightBlue;
    s.styles = "4";
    return s;
}

climd_style climd_link_url() {
    climd_style s;
    s.fg = kBlue;
    s.bg256 = 235;
    return s;
}

climd_style climd_image_alt() {
    climd_style s;
    s.fg = kBrightMagenta;
    s.styles = "3";
    return s;
}

climd_style climd_image_src() {
    climd_style s;
    s.fg = kMagenta;
    s.bg256 = 235;
    return s;
}

climd_style climd_blockquote_marker() {
    climd_style s;
    s.fg = kBrightBlack;
    s.prefix = "▌ ";
    return s;
}

climd_style climd_blockquote_text() {
    climd_style s;
    s.fg = kGrayLight;
    s.bg256 = 235;
    s.styles = "3";
    return s;
}

climd_style climd_list_marker() {
    climd_style s;
    s.fg = kBrightYellow;
    s.styles = "1";
    return s;
}

climd_style climd_task_checked() {
    climd_style s;
    s.fg = kBrightGreen;
    s.styles = "1";
    return s;
}

climd_style climd_task_unchecked() {
    climd_style s;
    s.fg = kBrightRed;
    s.styles = "1";
    return s;
}

climd_style climd_hr() {
    climd_style s;
    s.fg = kBrightBlack;
    s.styles = "2";
    s.char_field = "─";
    return s;
}

climd_style climd_table_border() {
    climd_style s;
    s.fg = kCyan;
    s.styles = "2";
    return s;
}

climd_style climd_table_header() {
    climd_style s;
    s.fg = kBrightCyan;
    s.bg256 = 24;
    s.styles = "1";
    return s;
}

climd_style climd_table_cell() {
    return climd_style{};
}

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

bool climd_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

// One UTF-8 code point's byte length (malformed lead bytes count as 1).
size_t climd_utf8_len(unsigned char lead) {
    if (lead < 0x80u) {
        return 1;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

size_t climd_utf8_next(kimix::string_view text, size_t i) {
    if (i >= text.size()) {
        return i;
    }
    const size_t len = climd_utf8_len(static_cast<unsigned char>(text[i]));
    return i + len > text.size() ? text.size() : i + len;
}

// The reference's _strip_ansi (CSI + the common escape shapes).
kimix::string climd_strip_ansi(kimix::string_view text) {
    kimix::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == '[') {
                // CSI: params (0x30-0x3F), intermediates (0x20-0x2F), final
                // (0x40-0x7E).
                size_t j = i + 2;
                while (j < text.size() && text[j] >= 0x30 && text[j] <= 0x3F) {
                    ++j;
                }
                while (j < text.size() && text[j] >= 0x20 && text[j] <= 0x2F) {
                    ++j;
                }
                if (j < text.size() && text[j] >= 0x40 && text[j] <= 0x7E) {
                    i = j + 1;
                    continue;
                }
            } else if (next == ']') {
                // OSC terminated by BEL or ESC backslash.
                size_t j = i + 2;
                while (j < text.size() && text[j] != '' && text[j] != '') {
                    ++j;
                }
                if (j < text.size() && text[j] == '') {
                    i = j + 1;
                    continue;
                }
                if (j + 1 < text.size() && text[j] == '' && text[j + 1] == '\\') {
                    i = j + 2;
                    continue;
                }
            } else if (next >= 0x40 && next <= 0x5F) {
                i = i + 2; // two-byte Fe escape
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

size_t climd_visible_length(kimix::string_view text) {
    size_t count = 0;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b') {
            // Skip one escape (same shapes as the stripper).
            if (i + 1 < text.size() && text[i + 1] == '[') {
                size_t j = i + 2;
                while (j < text.size() && text[j] >= 0x30 && text[j] <= 0x3F) {
                    ++j;
                }
                while (j < text.size() && text[j] >= 0x20 && text[j] <= 0x2F) {
                    ++j;
                }
                if (j < text.size() && text[j] >= 0x40 && text[j] <= 0x7E) {
                    i = j + 1;
                    continue;
                }
            }
            if (i + 1 < text.size() && text[i + 1] >= 0x40 && text[i + 1] <= 0x5F) {
                i += 2;
                continue;
            }
        }
        i = climd_utf8_next(text, i);
        ++count;
    }
    return count;
}

// The style application the reference expresses as colorful_text(text, **spec).
kimix::string climd_style_text(kimix::string_view text, const climd_style &spec) {
    if (spec.bg256 >= 0) {
        return colorful_text_256(text, spec.fg, spec.bg256, spec.styles);
    }
    return colorful_text(text, spec.fg, -1, spec.styles);
}

// ---------------------------------------------------------------------------
// Inline highlighting (utils.py:213-307)
// ---------------------------------------------------------------------------

// Split on inline code spans (`...`) -> (is_code, segment) pairs.
void climd_split_inline_code(kimix::string_view text,
                             kimix::vector<std::pair<bool, kimix::string>> &parts) {
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '`') {
            const size_t close = kimix::cli::find(text, "`", i + 1);
            if (close != kimix::string_view::npos) {
                if (i > 0) {
                    parts.emplace_back(false, kimix::string(text.substr(0, i)));
                }
                parts.emplace_back(true, kimix::string(text.substr(i + 1, close - i - 1)));
                const size_t consumed = close + 1;
                kimix::string rest(text.substr(consumed));
                kimix::vector<std::pair<bool, kimix::string>> tail;
                climd_split_inline_code(rest, tail);
                for (auto &p : tail) {
                    parts.push_back(std::move(p));
                }
                return;
            }
        }
        ++i;
    }
    if (!text.empty()) {
        parts.emplace_back(false, kimix::string(text));
    }
}

// Find the closing marker of a run starting at `pos` (`marker` is 2-3 chars).
// Returns the position of the closing marker or npos.
size_t climd_find_close(kimix::string_view text, size_t pos, kimix::string_view marker) {
    const size_t search = pos + marker.size(); // (unchanged)
    if (search > text.size()) {
        return kimix::string_view::npos;
    }
    return kimix::cli::find(text, marker, search);
}

// The style passes of _md_apply_inline_styles, in the reference order (image,
// link, strike, bold-italic, bold, italic; underscore emphasis is NOT
// transformed - utils.py:256-260).
kimix::string climd_apply_inline_styles(kimix::string_view segment) {
    kimix::string out(segment);
    bool changed = true;
    while (changed) { // image/link/strike/bold-italic/bold/italic, one at a time
        changed = false;
        // ![alt](src)
        const size_t bang = kimix::cli::find(out, "![");
        if (bang != kimix::string::npos) {
            const size_t alt_end = find(out, "]", bang + 2);
            if (alt_end != kimix::string::npos && alt_end + 1 < out.size() &&
                out[alt_end + 1] == '(') {
                const size_t src_end = find(out, ")", alt_end + 2);
                if (src_end != kimix::string::npos) {
                    const kimix::string alt = out.substr(bang + 2, alt_end - bang - 2);
                    const kimix::string src = out.substr(alt_end + 2, src_end - alt_end - 2);
                    kimix::string repl =
                        climd_style_text(alt, climd_image_alt()) + ": " +
                        climd_style_text(src, climd_image_src());
                    out = out.substr(0, bang) + repl + out.substr(src_end + 1);
                    changed = true;
                    continue;
                }
            }
        }
        // [text](url) (not preceded by '!')
        for (size_t pos = kimix::cli::find(out, "["); pos != kimix::string::npos;
             pos = kimix::cli::find(out, "[", pos + 1)) {
            if (pos > 0 && out[pos - 1] == '!') {
                continue;
            }
            const size_t text_end = find(out, "]", pos + 1);
            if (text_end == kimix::string::npos || text_end + 1 >= out.size() ||
                out[text_end + 1] != '(') {
                continue;
            }
            const size_t url_end = find(out, ")", text_end + 2);
            if (url_end == kimix::string::npos) {
                continue;
            }
            const kimix::string link = out.substr(pos + 1, text_end - pos - 1);
            const kimix::string url = out.substr(text_end + 2, url_end - text_end - 2);
            kimix::string repl =
                climd_style_text(link, climd_link_text()) + " (" +
                climd_style_text(url, climd_link_url()) + ")";
            out = out.substr(0, pos) + repl + out.substr(url_end + 1);
            changed = true;
            break;
        }
        if (changed) {
            continue;
        }
        // ~~text~~
        const size_t strike = kimix::cli::find(out, "~~");
        if (strike != kimix::string::npos) {
            const size_t close = climd_find_close(out, strike, "~~");
            if (close != kimix::string::npos) {
                const kimix::string body = out.substr(strike + 2, close - strike - 2);
                if (!body.empty()) {
                    out = out.substr(0, strike) +
                          climd_style_text(body, climd_strikethrough()) +
                          out.substr(close + 2);
                    changed = true;
                    continue;
                }
            }
        }
        // ***text*** / ___text___
        for (const char *marker : {"***", "___"}) {
            const size_t open = kimix::cli::find(out, marker);
            if (open != kimix::string::npos) {
                const size_t close = climd_find_close(out, open, marker);
                if (close != kimix::string::npos) {
                    const kimix::string body = out.substr(open + 3, close - open - 3);
                    if (!body.empty()) {
                        climd_style spec;
                        spec.fg = kBrightWhite;
                        spec.styles = "1,3";
                        out = out.substr(0, open) + climd_style_text(body, spec) +
                              out.substr(close + 3);
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (changed) {
            continue;
        }
        // **text** / __text__
        for (const char *marker : {"**", "__"}) {
            const size_t open = kimix::cli::find(out, marker);
            if (open != kimix::string::npos) {
                const size_t close = climd_find_close(out, open, marker);
                if (close != kimix::string::npos) {
                    const kimix::string body = out.substr(open + 2, close - open - 2);
                    if (!body.empty()) {
                        climd_style spec;
                        spec.fg = kBrightWhite;
                        spec.styles = "1";
                        out = out.substr(0, open) + climd_style_text(body, spec) +
                              out.substr(close + 2);
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (changed) {
            continue;
        }
        // *text* only (no underscore italic; utils.py:256-260).
        const size_t open = kimix::cli::find(out, "*");
        if (open != kimix::string::npos) {
            const size_t close = kimix::cli::find(out, "*", open + 1);
            if (close != kimix::string::npos && close > open + 1) {
                const kimix::string body = out.substr(open + 1, close - open - 1);
                climd_style spec;
                spec.fg = kBrightCyan;
                spec.styles = "3";
                out = out.substr(0, open) + climd_style_text(body, spec) +
                      out.substr(close + 1);
                changed = true;
            }
        }
    }
    return out;
}

// _md_highlight_inline: inline code spans styled, non-code segments passed
// through the style passes.
kimix::string climd_highlight_inline(kimix::string_view text) {
    kimix::vector<std::pair<bool, kimix::string>> parts;
    climd_split_inline_code(text, parts);
    if (parts.empty()) {
        return {};
    }
    climd_style inline_code;
    inline_code.fg = kYellow;
    inline_code.bg256 = 236;
    kimix::string out;
    for (const auto &part : parts) {
        if (part.first) {
            out += climd_style_text(part.second, inline_code);
        } else {
            out += climd_apply_inline_styles(part.second);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ANSI-aware wrapping (utils.py:545-593 paragraph, :396-467 blockquote)
// ---------------------------------------------------------------------------

// Greedy word wrap over the PLAIN text of `text`; the recorded break
// positions are then applied to the ANSI text (utils.py:556-593).  This is
// the reference's textwrap.fill(break_long_words=False, drop_whitespace=False,
// replace_whitespace=False) equivalent.
kimix::string climd_wrap_ansi(kimix::string_view text, int width) {
    const kimix::string plain = climd_strip_ansi(text);
    if (width <= 0 || static_cast<int>(climd_visible_length(text)) <= width) {
        return kimix::string(text);
    }
    // Break positions: greedy fill of the plain text.
    kimix::vector<size_t> breaks; // visible indices where a newline goes
    {
        size_t line_start = 0;
        size_t i = 0;
        const size_t n = plain.size();
        while (i < n) {
            // One word.
            size_t word_end = i;
            while (word_end < n && plain[word_end] != ' ' && plain[word_end] != '\n') {
                word_end = climd_utf8_next(plain, word_end);
            }
            const size_t word_width = static_cast<size_t>(climd_visible_length(
                kimix::string_view(plain).substr(i, word_end - i)));
            const size_t line_width = static_cast<size_t>(climd_visible_length(
                kimix::string_view(plain).substr(line_start, i - line_start)));
              if (i > line_start && line_width + word_width > static_cast<size_t>(width)) {
                  // The reference (_md_render_paragraph) records break positions
                  // as VISIBLE CHARACTER indices (its vis_idx counts code points
                  // of wrapped_plain), and the emit loop below matches them
                  // against its visible code-point counter. Store the visible
                  // length of the plain prefix, not the byte offset: with any
                  // multi-byte UTF-8 before the break the two differ, and a byte
                  // offset makes the break never fire (the line overflows the
                  // width and splits mid-word).
                  breaks.push_back(climd_visible_length(
                      kimix::string_view(plain).substr(0, i)));
                line_start = i;
                // The space at the break stays in the text (drop_whitespace is
                // false), so the next line starts after it.
                while (line_start < n && plain[line_start] == ' ') {
                    ++line_start;
                }
                i = line_start;
                continue;
            }
            if (word_end == i) {
                word_end = climd_utf8_next(plain, word_end); // a space or stuck char
            }
            i = word_end;
        }
    }
    // Insert newlines at the recorded visible positions of the ANSI text.
    kimix::string out;
    out.reserve(text.size() + breaks.size());
    size_t vis = 0;
    size_t next_break = 0;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b') {
            const size_t end = kimix::cli::find(text, "m", i + 1);
            const size_t stop = end == kimix::string_view::npos ? text.size() : end + 1;
            out.append(text.substr(i, stop - i));
            i = stop;
            continue;
        }
        if (next_break < breaks.size() && vis == breaks[next_break]) {
            out.push_back('\n');
            ++next_break;
        }
        const size_t next = climd_utf8_next(text, i);
        out.append(text.substr(i, next - i));
        i = next;
        ++vis;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Block renderers
// ---------------------------------------------------------------------------

kimix::string climd_render_bar(int width, const char *bar_char) {
    kimix::string bar;
    for (int i = 0; i < width; ++i) {
        bar += bar_char;
    }
    return climd_style_text(bar, climd_hr());
}

kimix::string climd_render_heading(int level, const kimix::string &content, int width,
                                   bool heading_bars) {
    const climd_style spec = climd_theme(level);
    const kimix::string highlighted = climd_highlight_inline(content);
    const kimix::string body = climd_style_text(
        kimix::string(spec.prefix) + highlighted + kimix::string(spec.suffix), spec);
    kimix::string out;
    if (heading_bars && spec.top_bar && spec.bar_char[0] != '\0') {
        out += climd_render_bar(width, spec.bar_char);
        out += "\n";
    }
    out += body;
    if (heading_bars && spec.bottom_bar && spec.bar_char[0] != '\0') {
        out += "\n";
        out += climd_render_bar(width, spec.bar_char);
    }
    return out;
}

kimix::string climd_render_list(const kimix::string &indent, const kimix::string &marker_raw,
                                const kimix::string &content) {
    const kimix::string marker =
        climd_style_text(indent + marker_raw + " ", climd_list_marker());
    return marker + climd_highlight_inline(content);
}

kimix::string climd_render_task(const kimix::string &indent, const kimix::string &bullet,
                                bool checked, const kimix::string &content) {
    const climd_style spec = checked ? climd_task_checked() : climd_task_unchecked();
    const kimix::string checkbox = checked ? "[x]" : "[ ]";
    const kimix::string marker =
        climd_style_text(indent + bullet + " " + checkbox + " ", spec);
    return marker + climd_highlight_inline(content);
}

kimix::string climd_render_blockquote(const kimix::string &line, int width) {
    // "> " prefix (one space optional: ^>\s?(.*)$).
    kimix::string content = line.substr(1);
    if (!content.empty() && content[0] == ' ') {
        content = content.substr(1);
    }
    const climd_style marker_spec = climd_blockquote_marker();
    const kimix::string marker = climd_style_text(marker_spec.prefix, marker_spec);
    const size_t marker_vis = climd_visible_length(marker_spec.prefix);
    kimix::string highlighted = climd_highlight_inline(content);
    const climd_style bq = climd_blockquote_text();
    kimix::string bq_prefix;
    if (bq.bg256 >= 0) {
        bq_prefix = ansi_prefix_256(bq.fg, bq.bg256, bq.styles);
    } else {
        bq_prefix = ansi_prefix(bq.fg, -1, bq.styles);
    }
    if (!bq_prefix.empty()) {
        // After each ANSI reset, re-apply the blockquote style so bg/italic
        // persists across inline spans (utils.py:411-417).
        kimix::string reapply = highlighted;
        kimix::string search("\x1b[0m");
        size_t pos = kimix::cli::find(reapply, search);
        while (pos != kimix::string::npos) {
            reapply = reapply.substr(0, pos + search.size()) + bq_prefix +
                      reapply.substr(pos + search.size());
            pos = kimix::cli::find(reapply, search, pos + search.size() + bq_prefix.size());
        }
        highlighted = bq_prefix + reapply + "\x1b[0m";
    }
    kimix::string text = highlighted;
    // Wrap the content to (width - marker), then indent the continuation lines.
    if (width > 0) {
        const int content_width =
            static_cast<int>(width) > static_cast<int>(marker_vis)
                ? static_cast<int>(width - marker_vis)
                : 10;
        text = climd_wrap_ansi(text, content_width > 10 ? content_width : 10);
    }
    if (contains(text, "\n")) {
        kimix::string indent(static_cast<size_t>(marker_vis), ' ');
        kimix::vector<kimix::string> lines;
        split_lines(text, lines);
        kimix::string out = marker + (lines.empty() ? kimix::string() : lines[0]);
        for (size_t i = 1; i < lines.size(); ++i) {
            out += "\n" + indent + lines[i];
        }
        return out;
    }
    return marker + text;
}

kimix::string climd_render_hr(int width) {
    kimix::string bar;
    for (int i = 0; i < width; ++i) {
        bar += "─";
    }
    return climd_style_text(bar, climd_hr());
}

// Pipe-table cells (utils.py:477-484).
void climd_split_table_cells(const kimix::string &row, kimix::vector<kimix::string> &cells) {
    kimix::string r = kimix::string(trim(row));
    if (!r.empty() && r.front() == '|') {
        r = r.substr(1);
    }
    if (!r.empty() && r.back() == '|') {
        r = r.substr(0, r.size() - 1);
    }
    split(r, '|', cells, true);
    for (kimix::string &cell : cells) {
        cell = kimix::string(trim(cell));
    }
}

bool climd_is_table_border(const kimix::string &row) {
    // ^[\s|]*:?-+:?[\s|]*$ with at least one dash.
    size_t i = 0;
    while (i < row.size() && (row[i] == ' ' || row[i] == '\t' || row[i] == '|')) {
        ++i;
    }
    if (i < row.size() && row[i] == ':') {
        ++i;
    }
    size_t dashes = 0;
    while (i < row.size() && row[i] == '-') {
        ++i;
        ++dashes;
    }
    if (dashes == 0) {
        return false;
    }
    if (i < row.size() && row[i] == ':') {
        ++i;
    }
    while (i < row.size() && (row[i] == ' ' || row[i] == '\t' || row[i] == '|')) {
        ++i;
    }
    return i == row.size();
}

kimix::string climd_render_table(const kimix::vector<kimix::string> &lines) {
    kimix::vector<kimix::vector<kimix::string>> rows;
    for (const kimix::string &line : lines) {
        kimix::vector<kimix::string> cells;
        climd_split_table_cells(line, cells);
        rows.push_back(std::move(cells));
    }
    if (rows.empty()) {
        return {};
    }
    size_t max_cols = 0;
    for (const auto &row : rows) {
        max_cols = row.size() > max_cols ? row.size() : max_cols;
    }
    kimix::vector<size_t> col_widths(max_cols, 0);
    for (const auto &row : rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            const size_t w = climd_visible_length(row[i]);
            col_widths[i] = w > col_widths[i] ? w : col_widths[i];
        }
    }
    auto pad = [](const kimix::string &cell, size_t width) {
        kimix::string out = cell;
        while (climd_visible_length(out) < width) {
            out.push_back(' ');
        }
        return out;
    };
    auto render_row = [&](const kimix::vector<kimix::string> &row,
                          const climd_style &spec) {
        kimix::string out = climd_style_text("|", climd_table_border());
        for (size_t i = 0; i < max_cols; ++i) {
            const kimix::string raw = i < row.size() ? row[i] : kimix::string();
            out += climd_style_text(pad(raw, col_widths[i]), spec);
            out += climd_style_text("|", climd_table_border());
        }
        return out;
    };
    kimix::vector<kimix::string> rendered;
    for (size_t idx = 0; idx < rows.size(); ++idx) {
        if (idx == 0) {
            rendered.push_back(render_row(rows[idx], climd_table_header()));
        } else if (climd_is_table_border(lines[idx])) {
            kimix::string out = climd_style_text("+", climd_table_border());
            for (const size_t w : col_widths) {
                kimix::string dashes(w + 2, '-');
                out += climd_style_text(dashes, climd_table_border());
                out += climd_style_text("+", climd_table_border());
            }
            rendered.push_back(std::move(out));
        } else {
            rendered.push_back(render_row(rows[idx], climd_table_cell()));
        }
    }
    return join(rendered, "\n");
}

kimix::string climd_render_code_block(const kimix::string &fence, const kimix::string &info,
                                      const kimix::vector<kimix::string> &body) {
    kimix::vector<kimix::string> lines;
    kimix::string header = fence;
    if (!info.empty()) {
        header += " " + info;
    }
    lines.push_back(climd_style_text(header, climd_code_fence()));
    for (const kimix::string &line : body) {
        lines.push_back(climd_style_text(line, climd_code_block()));
    }
    lines.push_back(climd_style_text(fence, climd_code_fence()));
    return join(lines, "\n");
}

// The paragraph pass: highlight + wrap (utils.py:545-593).
kimix::string climd_render_paragraph(const kimix::string &text, int width) {
    kimix::string highlighted = climd_highlight_inline(text);
    if (width <= 0) {
        return highlighted;
    }
    return climd_wrap_ansi(highlighted, width);
}

// ---------------------------------------------------------------------------
// Block classification (utils.py:314-321)
// ---------------------------------------------------------------------------

// ^(#{1,6})\s+(.*)$
bool climd_match_heading(kimix::string_view line, int &level, kimix::string &content) {
    size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#' && hashes < 6) {
        ++hashes;
    }
    if (hashes == 0 || hashes >= line.size() || line[hashes] != ' ') {
        return false;
    }
    level = static_cast<int>(hashes);
    content = kimix::string(trim(line.substr(hashes + 1)));
    return true;
}

// ^([\s]*)([-*+])\s+(.*)$
bool climd_match_unordered(kimix::string_view line, kimix::string &indent,
                           kimix::string &bullet, kimix::string &content) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size() || (line[i] != '-' && line[i] != '*' && line[i] != '+')) {
        return false;
    }
    if (i + 1 >= line.size() || line[i + 1] != ' ') {
        return false;
    }
    indent = kimix::string(line.substr(0, i));
    bullet = kimix::string(1, line[i]);
    content = kimix::string(line.substr(i + 2));
    return true;
}

// ^([\s]*)(\d+)\.\s+(.*)$
bool climd_match_ordered(kimix::string_view line, kimix::string &indent,
                         kimix::string &number, kimix::string &content) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    const size_t digits_start = i;
    while (i < line.size() && climd_is_digit(line[i])) {
        ++i;
    }
    if (i == digits_start || i + 1 >= line.size() || line[i] != '.' ||
        line[i + 1] != ' ') {
        return false;
    }
    indent = kimix::string(line.substr(0, digits_start));
    number = kimix::string(line.substr(digits_start, i - digits_start));
    content = kimix::string(line.substr(i + 2));
    return true;
}

// ^([-*_])\s*\1\s*\1(?:\s*\1)*$
bool climd_is_hr(kimix::string_view line) {
    if (line.size() < 3) {
        return false;
    }
    const char ch = line[0];
    if (ch != '-' && ch != '*' && ch != '_') {
        return false;
    }
    int count = 0;
    for (const char c : line) {
        if (c == ch) {
            ++count;
        } else if (c != ' ') {
            return false;
        }
    }
    return count >= 3;
}

// ^(```|~~~)(.*)$
bool climd_match_fence(kimix::string_view line, kimix::string &fence, kimix::string &info) {
    if (line.size() < 3) {
        return false;
    }
    const char ch = line[0];
    if ((ch != '`' && ch != '~') || line[1] != ch || line[2] != ch) {
        return false;
    }
    fence = kimix::string(3, ch);
    info = kimix::string(trim(line.substr(3)));
    return true;
}

// _md_terminal_width: explicit width floored to 20, else the terminal query.
int climd_terminal_width(int width) {
    if (width >= 0) {
        return width < 20 ? 20 : width;
    }
    const int columns = terminal_columns(stdout);
    return columns < 20 ? 20 : columns;
}

} // namespace

kimix::string markdown_strip_ansi(kimix::string_view text) {
    return climd_strip_ansi(text);
}

size_t markdown_visible_length(kimix::string_view text) {
    return climd_visible_length(text);
}

kimix::string markdown_wrap_ansi(const kimix::string &text, int width) {
    return climd_wrap_ansi(text, width);
}

kimix::string render_markdown(const kimix::string &text, int width, bool heading_bars) {
    const int term_width = climd_terminal_width(width);
    kimix::vector<kimix::string> lines;
    split_lines(text, lines);
    if (lines.empty()) {
        lines.push_back(kimix::string());
    }
    kimix::vector<kimix::string> output;
    kimix::vector<kimix::string> paragraph;
    auto flush_paragraph = [&]() {
        if (!paragraph.empty()) {
            output.push_back(climd_render_paragraph(join(paragraph, " "), term_width));
            paragraph.clear();
        }
    };
    size_t idx = 0;
    while (idx < lines.size()) {
        const kimix::string &line = lines[idx];
        const kimix::string stripped = kimix::string(trim(line));
        // Code fence.
        kimix::string fence, info;
        if (climd_match_fence(stripped, fence, info)) {
            flush_paragraph();
            kimix::vector<kimix::string> body;
            ++idx;
            while (idx < lines.size()) {
                const kimix::string inner = kimix::string(trim(lines[idx]));
                if (starts_with(inner, fence)) {
                    ++idx;
                    break;
                }
                body.push_back(lines[idx]);
                ++idx;
            }
            output.push_back(climd_render_code_block(fence, info, body));
            continue;
        }
        // Heading.
        int level = 0;
        kimix::string heading_content;
        if (climd_match_heading(stripped, level, heading_content)) {
            flush_paragraph();
            output.push_back(
                climd_render_heading(level, heading_content, term_width, heading_bars));
            if (level <= 2) {
                output.push_back(kimix::string());
            }
            ++idx;
            continue;
        }
        // Horizontal rule.
        if (climd_is_hr(stripped)) {
            flush_paragraph();
            output.push_back(climd_render_hr(term_width));
            ++idx;
            continue;
        }
        // Task list item.
        kimix::string indent, bullet, number, content;
        if (climd_match_unordered(stripped, indent, bullet, content) && content.size() >= 3 &&
            content[0] == '[' && (content[1] == 'x' || content[1] == 'X' || content[1] == ' ') &&
            content[2] == ']') {
            flush_paragraph();
            const bool checked = content[1] == 'x' || content[1] == 'X';
            kimix::string rest = content.substr(3);
            if (!rest.empty() && rest[0] == ' ') {
                rest = rest.substr(1);
            }
            output.push_back(
                climd_render_task(indent, bullet, checked, rest));
            ++idx;
            continue;
        }
        // Unordered / ordered list item.
        if (climd_match_unordered(stripped, indent, bullet, content)) {
            flush_paragraph();
            output.push_back(climd_render_list(indent, bullet, content));
            ++idx;
            continue;
        }
        if (climd_match_ordered(stripped, indent, number, content)) {
            flush_paragraph();
            output.push_back(climd_render_list(indent, number + ".", content));
            ++idx;
            continue;
        }
        // Blockquote.
        if (!stripped.empty() && stripped[0] == '>') {
            flush_paragraph();
            output.push_back(climd_render_blockquote(line, term_width));
            ++idx;
            continue;
        }
        // Table.
        if (!stripped.empty() && stripped[0] == '|') {
            flush_paragraph();
            kimix::vector<kimix::string> table_lines;
            while (idx < lines.size()) {
                const kimix::string candidate = kimix::string(trim(lines[idx]));
                if (candidate.empty() || candidate[0] != '|') {
                    break;
                }
                table_lines.push_back(candidate);
                ++idx;
            }
            output.push_back(climd_render_table(table_lines));
            continue;
        }
        // Blank line.
        if (stripped.empty()) {
            flush_paragraph();
            output.push_back(kimix::string());
            ++idx;
            continue;
        }
        // Paragraph continuation.
        paragraph.push_back(kimix::string(trim(line)));
        ++idx;
    }
    flush_paragraph();
    kimix::string result = join(output, "\n");
    if (!ends_with(result, "\n")) {
        result += "\n";
    }
    return result;
}

} // namespace kimix::cli
