// cli/cli_print.cpp - implementation of the terminal printing layer.
//
// Unity build note: the raw style-code scanner lives in an anonymous namespace
// with the `clip_` prefix.

#include <cstring>

#include "cli/cli_print.h"
#include "cli/cli_common.h"

namespace kimix::cli {

namespace {

bool g_colorful = false;
bool g_colorful_initialised = false;
bool g_quiet = false;
std::FILE *g_plain_stream = nullptr;
bool g_last_char_was_newline = true;

// Split a raw style list ("1,4" / "1 4" / "1;4") into ANSI codes appended to
// `out`.  Separators mirror tool.h's alias separators so callers can use any of
// them.
void clip_append_styles(kimix::string &out, kimix::string_view styles) {
    size_t start = 0;
    for (size_t i = 0; i <= styles.size(); ++i) {
        const bool at_end = (i == styles.size());
        const char c = at_end ? '\0' : styles[i];
        const bool sep = at_end || c == ' ' || c == '\t' || c == ',' || c == ';' ||
                         c == '|' || c == '\n' || c == '\r';
        if (!sep) {
            continue;
        }
        if (i > start) {
            if (!out.empty()) {
                out.push_back(';');
            }
            out.append(styles.substr(start, i - start));
        }
        start = i + 1;
    }
}

// Tiny int-to-string helper kept local so the colour paths never depend on
// std::to_string's locale handling.
kimix::string clip_int(int value) {
    if (value == 0) {
        return kimix::string("0");
    }
    char buf[16] = {};
    int n = 0;
    int v = value;
    while (v > 0 && n < 15) {
        buf[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    kimix::string out;
    out.reserve(static_cast<size_t>(n));
    for (int i = n - 1; i >= 0; --i) {
        out.push_back(buf[i]);
    }
    return out;
}

void clip_append_code(kimix::string &out, int code) {
    if (code < 0) {
        return;
    }
    if (!out.empty()) {
        out.push_back(';');
    }
    out.append(clip_int(code));
}

kimix::string clip_wrap(kimix::string_view text, const kimix::string &codes) {
    if (!g_colorful || codes.empty() || text.empty()) {
        return kimix::string(text);
    }
    kimix::string out;
    out.reserve(text.size() + codes.size() + 5);
    out.append("\x1b[");
    out.append(codes);
    out.push_back('m');
    out.append(text);
    out.append("\x1b[0m");
    return out;
}

void clip_write(std::FILE *stream, kimix::string_view text) {
    if (text.empty()) {
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stream);
    g_last_char_was_newline = (text.back() == '\n');
}

} // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
void init_printing(bool no_color) {
    g_colorful_initialised = true;
    const bool ansi_ok = enable_console_ansi();
    g_colorful = !no_color && ansi_ok && stream_is_console(stdout);
}

void set_colorful(bool on) {
    g_colorful_initialised = true;
    g_colorful = on;
}

bool colorful() {
    if (!g_colorful_initialised) {
        // Lazy default: colour when attached to a console.
        g_colorful_initialised = true;
        const bool ansi_ok = enable_console_ansi();
        g_colorful = ansi_ok && stream_is_console(stdout);
    }
    return g_colorful;
}

void set_quiet(bool on) {
    g_quiet = on;
}

bool quiet() {
    return g_quiet;
}

void set_plain_stream(std::FILE *stream) {
    g_plain_stream = stream;
}

std::FILE *plain_stream() {
    return (g_plain_stream != nullptr) ? g_plain_stream : stderr;
}

// ---------------------------------------------------------------------------
// Colour construction
// ---------------------------------------------------------------------------
kimix::string ansi_prefix(int fg, int bg, kimix::string_view styles) {
    kimix::string codes;
    clip_append_styles(codes, styles);
    clip_append_code(codes, fg);
    clip_append_code(codes, bg);
    if (codes.empty()) {
        return {};
    }
    kimix::string out("\x1b[");
    out.append(codes);
    out.push_back('m');
    return out;
}

kimix::string ansi_prefix_256(int fg256, int bg256, kimix::string_view styles) {
    kimix::string codes;
    clip_append_styles(codes, styles);
    if (fg256 >= 0) {
        if (!codes.empty()) {
            codes.push_back(';');
        }
        codes.append("38;5;");
        codes.append(clip_int(fg256));
    }
    if (bg256 >= 0) {
        if (!codes.empty()) {
            codes.push_back(';');
        }
        codes.append("48;5;");
        codes.append(clip_int(bg256));
    }
    if (codes.empty()) {
        return {};
    }
    kimix::string out("\x1b[");
    out.append(codes);
    out.push_back('m');
    return out;
}

kimix::string ansi_prefix_true(int r, int g, int b, kimix::string_view styles) {
    kimix::string codes;
    clip_append_styles(codes, styles);
    if (r >= 0 && g >= 0 && b >= 0) {
        if (!codes.empty()) {
            codes.push_back(';');
        }
        codes.append("38;2;");
        codes.append(clip_int(r));
        codes.push_back(';');
        codes.append(clip_int(g));
        codes.push_back(';');
        codes.append(clip_int(b));
    }
    if (codes.empty()) {
        return {};
    }
    kimix::string out("\x1b[");
    out.append(codes);
    out.push_back('m');
    return out;
}

kimix::string colorful_text(kimix::string_view text, int fg, int bg,
                            kimix::string_view styles) {
    return clip_wrap(text, ansi_prefix(fg, bg, styles));
}

kimix::string colorful_text_256(kimix::string_view text, int fg256, int bg256,
                                kimix::string_view styles) {
    return clip_wrap(text, ansi_prefix_256(fg256, bg256, styles));
}

kimix::string colorful_text_true(kimix::string_view text, int r, int g, int b,
                                 kimix::string_view styles) {
    return clip_wrap(text, ansi_prefix_true(r, g, b, styles));
}

kimix::string gray_text(kimix::string_view text) {
    return colorful_text_256(text, kGray256, -1);
}

kimix::string gray_light_text(kimix::string_view text) {
    return colorful_text_256(text, kGrayLight256, -1);
}

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------
void print_string(kimix::string_view text) {
    clip_write(stdout, text);
    clip_write(stdout, "\n");
}

void print_raw(kimix::string_view text) {
    clip_write(stdout, text);
}

void print_info(kimix::string_view text) {
    const kimix::string line =
        colorful_text(text, static_cast<int>(color::bright_magenta));
    clip_write(stdout, line);
    clip_write(stdout, "\n");
}

void print_success(kimix::string_view text) {
    const kimix::string line = colorful_text(
        text, static_cast<int>(color::bright_green),
        -1, "1");
    clip_write(stdout, line);
    clip_write(stdout, "\n");
}

void print_error(kimix::string_view text) {
    const kimix::string line = colorful_text(
        text, static_cast<int>(color::bright_red),
        -1, "1");
    clip_write(plain_stream(), line);
    clip_write(plain_stream(), "\n");
}

void print_warning(kimix::string_view text) {
    const kimix::string line = colorful_text(
        text, static_cast<int>(color::bright_yellow),
        -1, "1");
    clip_write(stdout, line);
    clip_write(stdout, "\n");
}

void print_debug(kimix::string_view text) {
    if (g_quiet) {
        return;
    }
    const kimix::string line =
        colorful_text(text, static_cast<int>(color::bright_cyan));
    clip_write(stdout, line);
    clip_write(stdout, "\n");
}

void print_word(kimix::string_view word, bool require_new_line, bool flush) {
    if (require_new_line && !g_last_char_was_newline) {
        clip_write(stdout, "\n");
    }
    clip_write(stdout, word);
    if (flush) {
        std::fflush(stdout);
    }
}

bool last_char_was_newline() {
    return g_last_char_was_newline;
}

void reset_print_state() {
    g_last_char_was_newline = true;
}

} // namespace kimix::cli
