// cli/cli_print.h - Terminal printing layer: the C++ port of kimi-agent's
// kimix/ui/printing.py.
//
// The Python module exposes module-level state (_colorful_print, _quiet,
// _print_func, _stream) plus a small colour vocabulary (Color / BgColor / Style
// enums, Color256 / TrueColor wrappers, the GRAY / GRAY_LIGHT named colours) and
// the print_* helpers.  This header keeps the same shape in namespace
// kimix::cli with process-wide state instead of Python globals.
//
// Colour handling (identical to the reference):
//   * `ansi_prefix` builds "\x1b[" + styles + ";" + fg + ";" + bg + "m", with
//     the codes joined in that order and empty parts skipped;
//   * `colorful_text` returns the text UNCHANGED when colour is disabled, so a
//     caller never has to branch;
//   * print_info -> bright magenta on stdout,
//     print_success -> bright green + bold, print_warning -> bright yellow +
//     bold, print_debug -> bright cyan (muted when quiet),
//     print_error -> bright red + bold on stderr.

#pragma once

#include <cstdint>
#include <cstdio>
#include <core/kimix_core.h>

namespace kimix::cli {

// ---------------------------------------------------------------------------
// Colour vocabulary (printing.py: Color / BgColor / Style)
// ---------------------------------------------------------------------------
enum class color : int {
    black = 30,
    red = 31,
    green = 32,
    yellow = 33,
    blue = 34,
    magenta = 35,
    cyan = 36,
    white = 37,
    bright_black = 90,
    bright_red = 91,
    bright_green = 92,
    bright_yellow = 93,
    bright_blue = 94,
    bright_magenta = 95,
    bright_cyan = 96,
    bright_white = 97,
};

enum class bg_color : int {
    black = 40,
    red = 41,
    green = 42,
    yellow = 43,
    blue = 44,
    magenta = 45,
    cyan = 46,
    white = 47,
    bright_black = 100,
    bright_red = 101,
    bright_green = 102,
    bright_yellow = 103,
    bright_blue = 104,
    bright_magenta = 105,
    bright_cyan = 106,
    bright_white = 107,
};

enum class style : int {
    reset = 0,
    bold = 1,
    dim = 2,
    italic = 3,
    underline = 4,
    blink = 5,
    inverse = 7,
    hidden = 8,
    strike = 9,
};

// printing.py's named greys (Color256 indices).
constexpr int kGray256 = 245;
constexpr int kGrayLight256 = 250;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Decide the initial colour setting: colour is on when stdout is a console and
// ANSI escapes could be enabled, off when `no_color` is requested or the output
// is redirected.  Also configures the Windows console (UTF-8 + VT processing).
void init_printing(bool no_color);

void set_colorful(bool on);
bool colorful();
void set_quiet(bool on);
bool quiet();
// Stream used by print_error (default stderr).
void set_plain_stream(std::FILE *stream);
std::FILE *plain_stream();

// ---------------------------------------------------------------------------
// Colour construction
// ---------------------------------------------------------------------------
// "\x1b[<styles>;<fg>;<bg>m" - `fg`/`bg` are the raw enum values, -1 == none.
kimix::string ansi_prefix(int fg, int bg, kimix::string_view styles = {});
// 256-colour form: 38;5;N (fg) / 48;5;N (bg).
kimix::string ansi_prefix_256(int fg256, int bg256, kimix::string_view styles = {});
// True-colour form: 38;2;r;g;b.
kimix::string ansi_prefix_true(int r, int g, int b, kimix::string_view styles = {});

// `text` wrapped in the escape sequence + "\x1b[0m"; unchanged when colour is
// off.  The optional `styles` list holds raw style codes separated by space,
// comma, ';' or '|' (e.g. "1" for bold, "1,4" for bold+underline).
kimix::string colorful_text(kimix::string_view text, int fg = -1, int bg = -1,
                            kimix::string_view styles = {});
kimix::string colorful_text_256(kimix::string_view text, int fg256, int bg256,
                                kimix::string_view styles = {});
kimix::string colorful_text_true(kimix::string_view text, int r, int g, int b,
                                 kimix::string_view styles = {});
// Convenience wrappers over the named greys used by the stream layer.
kimix::string gray_text(kimix::string_view text);
kimix::string gray_light_text(kimix::string_view text);

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------
void print_string(kimix::string_view text);  // plain, newline-terminated
void print_raw(kimix::string_view text);     // no colour, no newline
void print_info(kimix::string_view text);
void print_success(kimix::string_view text);
void print_error(kimix::string_view text);  // -> plain_stream() (stderr)
void print_warning(kimix::string_view text);
void print_debug(kimix::string_view text);  // muted when quiet()
// PrintStream.print_word(word, require_new_line, raw_word=None, flush=False):
// writes `word` without a trailing newline, first emitting a bare '\n' when
// `require_new_line` is set and the last printed character was not one.
void print_word(kimix::string_view word, bool require_new_line, bool flush = false);
// The runtime state PrintStream keeps (printing.py PrintStream._last_char_was_newline).
bool last_char_was_newline();
void reset_print_state();

} // namespace kimix::cli
