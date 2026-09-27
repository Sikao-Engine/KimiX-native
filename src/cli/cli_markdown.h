// cli/cli_markdown.h - ANSI terminal markdown renderer (H4).
//
// Port of kimix/cli_impl/utils.py's render_markdown (utils.py:63-725): the
// DEFAULT_MD_THEME element specs, the inline pass (escape placeholders, inline
// code split, image/link/strike/bold-italic/bold/italic with the deliberate
// no-underscore-italic rule), the block renderers (headings with full-width
// bars, lists with [x]/[ ] colouring, blockquote with the "▌ " marker and the
// per-reset style re-application, hr, pipe tables padded to visible width,
// fenced code with the `lang` header) and the ANSI-aware paragraph wrap.
//
// Wrapping width: the reference's _md_terminal_width (utils.py:324-331) - an
// explicit width floored to 20, else shutil.get_terminal_size().columns or 80
// (cli_common's terminal_columns supplies the same COLUMNS/env -> console ->
// 80 chain).
//
// Colour goes through cli_print's colorful_text*/gray_text helpers, so the
// global --no_color / console detection state decides whether escapes are
// emitted at all (printing.py:339-354 parity: colourful_text returns the text
// unchanged when colour is off - the structural rendering still applies).
//
// Rules (see .agents/skills/cpp): namespace kimix::cli, kimix:: containers,
// no exceptions, no RTTI, unity build (TU-local helpers under `climd_`).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::cli {

// Render `text` (UTF-8 markdown) to ANSI-styled terminal output.  `width` < 0
// queries the terminal (terminal_columns(stdout) with the reference's 80
// fallback and the max(width, 20) floor); `heading_bars` toggles the h1/h2
// ═/─ rules.  The result always ends with exactly one "\n".
kimix::string render_markdown(const kimix::string &text, int width = -1,
                              bool heading_bars = true);

// The paragraph/blockquote wrapper the renderer uses: ANSI-aware greedy word
// wrap to `width` (already floored by the caller).  Exposed for tests.
kimix::string markdown_wrap_ansi(const kimix::string &text, int width);

// The visible width of a UTF-8 string, skipping ANSI escape sequences.
size_t markdown_visible_length(kimix::string_view text);

// Strip ANSI escape sequences (the reference's _strip_ansi).
kimix::string markdown_strip_ansi(kimix::string_view text);

} // namespace kimix::cli
